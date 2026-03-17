#include "rocket/common/msg_id_util.h"
#include "rocket/common/log.h"
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>

namespace rocket {

static constexpr int kMsgIdLength = 20;

static thread_local std::string t_msg_id_no;
static thread_local std::string t_max_msg_id_no;

namespace {

std::mt19937_64& GetRng() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng;
}

} // namespace

std::string MsgIDUtil::GenMsgID() {
    if (t_msg_id_no.empty() || t_msg_id_no == t_max_msg_id_no) {
        if (t_max_msg_id_no.empty()) {
            t_max_msg_id_no.assign(kMsgIdLength, '9');
        }

        auto& rng = GetRng();
        std::uniform_int_distribution<int> dist(0, 9);

        std::string res;
        res.reserve(kMsgIdLength);
        for (int i = 0; i < kMsgIdLength; ++i) {
            res += static_cast<char>('0' + dist(rng));
        }

        t_msg_id_no = std::move(res);
    } else {
        std::size_t i = t_msg_id_no.length();
        while (i > 0 && t_msg_id_no[i - 1] == '9') {
            --i;
        }

        if (i > 0) {
            ++t_msg_id_no[i - 1];
            for (std::size_t j = i; j < t_msg_id_no.length(); ++j) {
                t_msg_id_no[j] = '0';
            }
        }
    }

    return t_msg_id_no;
}

} // namespace rocket