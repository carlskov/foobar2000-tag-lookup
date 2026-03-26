#pragma once

#include "album_art_service.h"

#include <optional>
#include <vector>

namespace taglookup {

std::optional<size_t> SelectAlbumArtCandidateIndex(const AlbumArtQuery& query,
                                                   const std::vector<AlbumArtCandidate>& matches);

}  // namespace taglookup
