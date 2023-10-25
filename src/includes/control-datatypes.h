#ifndef DINIT_CONTROL_DATATYPES_H
#define DINIT_CONTROL_DATATYPES_H

#include <cstdint>

namespace dinit_cptypes {
    // A mapping between service records and their associated numerical identifier
    // used in communction
    using handle_t = uint32_t;
    using trigger_val_t = uint8_t;
    using cp_cmd_t = uint8_t;
    using cp_rply_t = uint8_t;
    using cp_info_t = uint8_t;
    using srvname_len_t = uint16_t;
} // namespace dinit_cptypes

#endif
