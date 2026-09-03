#include "library/subsonic/subsonicimportprogress.h"

namespace subsonic {

bool ImportProgress::onProgress(
        std::size_t tracksLoaded,
        std::size_t albumsDone,
        std::size_t albumsTotal) {
    if (m_onProgressFn) {
        m_onProgressFn(tracksLoaded, albumsDone, albumsTotal);
    }
    return !(m_pCancelFlag && m_pCancelFlag->load());
}

} // namespace subsonic
