#ifndef DINIT_CONTROL_DATATYPES_H_INCLUDED
#define DINIT_CONTROL_DATATYPES_H_INCLUDED 1

// A mapping between types used in service records and their associated underlying numerical type
// used in communication (control protocol)

#include <cstdint>

namespace dinit_cptypes {

using handle_t = uint32_t;
using trigger_val_t = uint8_t;
using cp_cmd_t = uint8_t;
using cp_rply_t = uint8_t;
using cp_info_t = uint8_t;
using srvname_len_t = uint16_t;
using envvar_len_t = uint16_t;
using sig_num_t = int;
using srvstate_t = uint8_t;

} // namespace dinit_cptypes

#endif // DINIT_CONTROL_DATATYPES_H_INCLUDED
