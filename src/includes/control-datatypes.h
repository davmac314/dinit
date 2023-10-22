#ifndef DINIT_CONTROL_DATATYPES_H
#define DINIT_CONTROL_DATATYPES_H

#include <cstdint>

namespace dinit_cptypes {
    // A mapping between service records and their associated numerical identifier
    // used in communction
    using handle_t = uint32_t;
    using trigger_val_t = uint8_t;
    using cp_cmd_t = uint8_t;
} // namespace dinit_cptypes

#endif
