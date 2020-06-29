#include "utils.h"

thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> t_xxhState(XXH3_createState(), XXH3_freeState);
