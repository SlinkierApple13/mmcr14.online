#pragma once

#include "qingque.h"

namespace qingque_wd {

    const qingque::w_data& get_wd();

    std::unordered_map<qingque::fan_code, double> fan_cache(const qingque::w_data& wd);

}
