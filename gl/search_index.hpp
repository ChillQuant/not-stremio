// Local search-as-you-type index: a compact prefix map over titles already on
// the device (library, continue watching, Discover cache, meta cache, recent
// progress). Built on a worker thread from a UI-thread snapshot, then swapped
// in atomically; lookups are microsecond binary searches, never touching the
// network. (SEARCH UX, worker 5.)

#pragma once

#include "model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sx
{

struct SearchHit
{
    Item item;
    std::string name_lc;              // lowercased once at build; lookups never re-lower
    std::vector<std::string> words;   // word-split once at build
    bool in_library = false;
    bool in_continue = false;
};

class SearchIndex
{
  public:
    // Everything the build needs, copied on the UI thread; the worker only
    // reads it. Sources are merged in priority order (first wins on dupes).
    struct Snapshot
    {
        std::vector<Item> library;
        std::vector<Item> continue_watching;
        std::vector<Item> discover;
        std::vector<Item> meta;
        std::vector<Item> recent;
    };

    static constexpr std::size_t kMaxTitles = 1500;
    static constexpr std::size_t kMaxPostings = 60000;
    static constexpr std::size_t kMaxResults = 6; // cards shown per keystroke

    // Worker thread: consumes the snapshot, builds the sorted postings.
    void build(Snapshot snap);

    // UI thread: hit indices into at(), best first, up to `limit`.
    std::vector<std::size_t> lookup(const std::string &query, std::size_t limit) const;

    const SearchHit &at(std::size_t i) const
    {
        return hits_[i];
    }
    bool empty() const
    {
        return hits_.empty();
    }
    std::size_t titles() const
    {
        return hits_.size();
    }
    std::size_t postings() const
    {
        return postings_.size();
    }
    std::size_t bytes() const; // estimated footprint, for the audit log

  private:
    std::vector<SearchHit> hits_;
    // (lowercased prefix, hit index), sorted: equal_range finds a prefix.
    std::vector<std::pair<std::string, std::uint32_t>> postings_;
};

} // namespace sx
