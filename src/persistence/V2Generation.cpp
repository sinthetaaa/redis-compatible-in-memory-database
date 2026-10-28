#include "redisdb/persistence/V2Generation.hpp"
#include "V2GenerationAccess.hpp"

#include <utility>

namespace redisdb::persistence::v2 {
using detail::GenerationAccess;

GenerationCandidate::~GenerationCandidate() noexcept { dispose(); }
void GenerationCandidate::dispose() noexcept {
    const auto released = GenerationAccess::release(*this);
    if (local_error_.code == RootErrorCode::None)
        local_error_ = released;
}
void GenerationCandidate::take(GenerationCandidate&& other) noexcept {
    root_ = std::move(other.root_);
    generation_ = std::exchange(other.generation_, std::nullopt);
    local_error_ = std::exchange(other.local_error_, RootError{});
}
GenerationCandidate::GenerationCandidate(GenerationCandidate&& other) noexcept {
    take(std::move(other));
}
GenerationCandidate& GenerationCandidate::operator=(GenerationCandidate&& other) noexcept {
    if (this != &other) {
        dispose();
        const auto previous = error();
        take(std::move(other));
        if (previous.code != RootErrorCode::None && previous.code != RootErrorCode::ClosedOrFailed)
            local_error_ = previous;
    }
    return *this;
}
GenerationCandidate GenerationCandidate::find_after(RootLease& root, GenerationId selected,
                                                    LogicalSequence sequence,
                                                    std::uint32_t budget) noexcept {
    GenerationCandidate candidate;
    if (budget == 0 || budget > 4096) {
        candidate.local_error_ = root.error();
        if (candidate.local_error_.code == RootErrorCode::None)
            candidate.local_error_ = {RootErrorCode::ResourceLimit};
        return candidate;
    }
    GenerationAccess::borrow(candidate, root);
    GenerationAccess::search(candidate, selected, sequence, budget);
    return candidate;
}
bool GenerationCandidate::valid() const noexcept {
    return generation_.has_value() && local_error_.code == RootErrorCode::None &&
           GenerationAccess::healthy(*this);
}
RootError GenerationCandidate::error() const noexcept {
    if (local_error_.code != RootErrorCode::None)
        return local_error_;
    const auto shared = GenerationAccess::error(*this);
    return shared.code != RootErrorCode::None ? shared
           : valid()                          ? RootError{}
                                              : RootError{RootErrorCode::ClosedOrFailed};
}
const GenerationId* GenerationCandidate::generation() const noexcept {
    return valid() ? &*generation_ : nullptr;
}
bool GenerationCandidate::recheck_unused() noexcept {
    return valid() && GenerationAccess::check(*this);
}
} // namespace redisdb::persistence::v2
