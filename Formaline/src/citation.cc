#include <cctk.h>
#include <cctk_Functions.h>

#include "citation.hh"

namespace Formaline {

namespace Citation {

using namespace std;

// registered 
static list<string> requested_keys, requested_items;
static list<string> suggested_keys, suggested_items;

// client thorn visible interface
extern "C"
void Formaline_RegisterRequested(CCTK_STRING key, CCTK_STRING item) {
  requested_keys.push_back(key);
  requested_items.push_back(item);
}

extern "C"
void Formaline_RegisterSuggested(CCTK_STRING key, CCTK_STRING item) {
  suggested_keys.push_back(key);
  suggested_items.push_back(item);
}

// Formaline visible interface
const list<string>& RequestedKeys() { return requested_keys; }
const list<string>& RequestedItems() { return requested_items; }

const list<string>& SuggestedKeys() { return suggested_keys; }
const list<string>& SuggestedItems() { return suggested_items; }

}

} // namespace Formaline
