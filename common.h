#ifndef COMMON_H
#define COMMON_H

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <system_error>

#include <infiniband/verbs.h>

constexpr size_t max_send_wr = 128;
constexpr size_t max_recv_wr = 512;
constexpr size_t alloc_alignment = 4096;

struct Bytes {
    size_t value;
};

struct ServerConnectionData {
    bool inline_data;
    uint64_t address;
    uint64_t size;
    uint32_t rkey;
};

struct ClientConnectionData {
    bool send;
    ssize_t locations;
};

template<typename T>
inline T align(T t, T a) {
    T mask = a - 1;
    return (t + mask) & ~mask;
}

inline std::ostream& operator<<(std::ostream& out, const Bytes& size) {
    out << size.value;
    return out;
}

inline std::istream& operator>>(std::istream& in, Bytes& size) {
    in >> size.value;
    char x;
    in >> x;
    size_t order = 0;
    switch(x) {
        case 'G':
            order++;
        case 'M':
            order++;
        case 'K':
            order++;
            break;
        default:
            break;
    }
    size.value *= static_cast<size_t>(1024) * order;
    return in;
}

const std::error_category& ibv_wc_error_category();

class ibv_wc_error_category_t : public std::error_category {
    private:
        ibv_wc_error_category_t() {}
    public:
        ~ibv_wc_error_category_t() override {};
        const char* name() const noexcept override {
            return "ibv_wc";
        }
        std::string message(int condition) const override {
            return ibv_wc_status_str(static_cast<ibv_wc_status>(condition));
        }
        friend const std::error_category& ibv_wc_error_category();
};

inline const std::error_category& ibv_wc_error_category() {
    static ibv_wc_error_category_t cat{};
    return cat;
}

#endif /* COMMON_H */
