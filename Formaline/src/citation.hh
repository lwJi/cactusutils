#ifndef FORMALINE_CITATION_HH
#define FORMALINE_CITATION_HH

#include <list>
#include <string>

namespace Formaline {

namespace Citation {

// this list of citation keys (short) and the (full) bibitems for requested
// citations
const std::list<std::string>& RequestedKeys();
const std::list<std::string>& RequestedItems();

// this list of citation keys (short) and the (full) bibitems for syggested
// citations
const std::list<std::string>& SuggestedKeys();
const std::list<std::string>& SuggestedItems();

} // namespace Citation

} // namespace Formaline

#endif /* #ifndef FORMALINE_CITATION_HH */
