#include "qingque_data.h"

namespace qingque {

    int w_data::get_a(int fan_id) const {
        auto it = a_table.find(fan_id);
        if (it != a_table.end()) {
            return it->second;
        }
        return 20; // Default value if fan_id not found
    }

}