#include <pgain/dedup.hpp>

namespace pgain {

bool Dedup::accept(uint64_t seq) {
    if (seq >= next_) {
        if (seq - next_ >= window)
            bits_.fill(0);
        else
            for (uint64_t s = next_; s < seq; s++)
                clear(s);
        set(seq);
        next_ = seq + 1;
        return true;
    }
    if (next_ - seq > window || test(seq))
        return false;
    set(seq);
    return true;
}

}
